#include "hc/api/ApiRouter.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace hc {

namespace {

struct Ctx {
  Ctx(HeatCoreSystem& s, const ApiRequest& r) : sys(s), req(r) {}
  HeatCoreSystem& sys;
  const ApiRequest& req;
  std::vector<std::string> p;  // path segments after "api"
  JsonDocument in;
  JsonDocument out;
  int status = 200;

  bool is(const char* m) const { return req.method == m; }
  int fail(int code, const std::string& msg) {
    out.clear();
    out["error"] = msg;
    status = code;
    return code;
  }
  int okOrFail(const std::string& err) {
    if (!err.empty()) return fail(400, err);
    out["ok"] = true;
    return 200;
  }
  std::string q(const char* key, const char* def = "") const {
    auto it = req.query.find(key);
    return it == req.query.end() ? def : it->second;
  }
  uint32_t qu(const char* key, uint32_t def) const {
    auto it = req.query.find(key);
    return it == req.query.end() ? def : static_cast<uint32_t>(std::strtoul(it->second.c_str(), nullptr, 10));
  }
  JsonObjectConst body() const { return in.as<JsonObjectConst>(); }
};

std::vector<std::string> splitPath(const std::string& path) {
  std::vector<std::string> v;
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') ++i;
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    if (j > i) v.push_back(path.substr(i, j - i));
    i = j;
  }
  return v;
}

int toInt(const std::string& s) { return std::atoi(s.c_str()); }

// ------------------------------------------------------------------ status

void statusJson(HeatCoreSystem& sys, JsonObject o) {
  const PlatformStatus& pl = sys.platform;
  o["device"] = sys.sysCfg.deviceName;
  o["deviceId"] = pl.deviceId;
  o["firmware"] = kFirmwareVersion;
  o["uptimeSec"] = pl.uptimeSec;
  o["epoch"] = sys.clock().epoch();
  LocalTime lt = sys.clock().local();
  if (lt.valid) {
    char b[24];
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d", lt.year, lt.month, lt.day, lt.hour, lt.minute);
    o["localTime"] = b;
  }
  o["timeValid"] = lt.valid;
  o["network"] = pl.network;
  o["ip"] = pl.ip;
  o["mqtt"] = pl.mqtt;
  o["globalMode"] = globalModeName(sys.heating.globalMode);
  o["outputs"] = sys.outputMask();
  o["alarmCount"] = static_cast<int>(sys.alarms.count());
  sys.alarms.toJson(o["alarms"].to<JsonArray>());
  JsonArray rooms = o["rooms"].to<JsonArray>();
  for (auto& r : sys.heating.rooms) {
    JsonObject x = rooms.add<JsonObject>();
    x["id"] = r.id;
    x["name"] = r.name;
    if (r.hasTemp) x["temperature"] = r.temperature;
    x["setpoint"] = r.setpoint;
    x["demand"] = r.demand;
    x["window"] = r.windowOpen();
    x["reason"] = r.reason;
    x["mode"] = roomModeName(r.mode);
    x["manual"] = r.manualActive;
    x["fault"] = r.sensorFail || r.overTemp;
  }
  JsonObject rf = o["rf"].to<JsonObject>();
  rf["devices"] = static_cast<int>(sys.rfReg.rx.size());
  rf["frames"] = sys.rfRx.stats().frames;
  rf["unknown"] = sys.rfRx.stats().unknown;
  rf["learning"] = sys.rfRx.learning();
  rf["txSent"] = sys.rfTx.stats().sent;
  JsonObject d = o["display"].to<JsonObject>();
  d["enabled"] = sys.display.cfg.enabled;
  d["ok"] = sys.display.hw.initialized;
  d["page"] = sys.display.page() + 1;
  JsonObject a = o["automation"].to<JsonObject>();
  a["rules"] = static_cast<int>(sys.automation.rules().size());
  a["executions"] = sys.automation.stats().executions;
  a["paused"] = sys.automation.paused();
  sys.safety.toJson(o["safety"].to<JsonObject>(), true);
  o["sensorsOk"] = static_cast<int>(sys.sensors.countAvailable());
  o["sensorsTotal"] = static_cast<int>(sys.sensors.sensors().size());
}

void hardwareJson(HeatCoreSystem& sys, JsonObject o) {
  const PlatformStatus& pl = sys.platform;
  JsonObject p = o["platform"].to<JsonObject>();
  p["ethLink"] = pl.ethLink;
  p["wifiConnected"] = pl.wifiConnected;
  p["wifiRssi"] = pl.wifiRssi;
  p["netDetail"] = pl.netDetail;
  p["freeHeap"] = pl.freeHeap;
  p["minFreeHeap"] = pl.minFreeHeap;
  p["freePsram"] = pl.freePsram;
  p["totalPsram"] = pl.totalPsram;
  p["cpuPct"] = pl.cpuPct;
  p["resetReason"] = pl.resetReason;
  p["rtcPresent"] = pl.rtcPresent;
  p["rtcValid"] = pl.rtcValid;
  p["rtcTempC"] = pl.rtcTempC;
  p["timeSynced"] = pl.timeSynced;
  p["sd"] = pl.sd;
  p["sdTotalBytes"] = pl.sdTotalBytes;
  p["sdUsedBytes"] = pl.sdUsedBytes;
  JsonArray i2c = p["i2c"].to<JsonArray>();
  for (uint8_t a : pl.i2cDevices) {
    char b[8];
    std::snprintf(b, sizeof(b), "0x%02X", a);
    i2c.add(b);
  }
  p["i2cErrors"] = pl.i2cErrors;
  p["i2cRecoveries"] = pl.i2cRecoveries;
  p["outputWriteErrors"] = pl.outputWriteErrors;
  p["inputReadErrors"] = pl.inputReadErrors;
  p["ioExpandersOk"] = pl.ioExpandersOk;
  p["controlLoopMaxUs"] = pl.controlLoopMaxUs;
  JsonArray ow = p["oneWire"].to<JsonArray>();
  for (uint8_t c : pl.oneWireCount) ow.add(c);
  p["rs485"] = pl.rs485;
  p["rs485Lines"] = pl.rs485Lines;
  p["rs485Errors"] = pl.rs485Errors;
  p["rfRxHw"] = pl.rfRxHw;
  p["rfTxHw"] = pl.rfTxHw;
  JsonArray st = o["selfTest"].to<JsonArray>();
  for (auto& t : sys.selfTest) {
    JsonObject x = st.add<JsonObject>();
    x["component"] = t.component;
    x["status"] = t.status;
    x["detail"] = t.detail;
  }
  JsonObject lb = o["rfLoopback"].to<JsonObject>();
  lb["running"] = sys.loopback.running;
  lb["done"] = sys.loopback.done;
  lb["ok"] = sys.loopback.ok;
  lb["echoes"] = sys.loopback.echoes;
  lb["detail"] = sys.loopback.detail;
  sys.io.toJson(o["io"].to<JsonObject>(), true);
}

// ------------------------------------------------------------------ RF

void rfStatusJson(HeatCoreSystem& sys, JsonObject o) {
  const auto& s = sys.rfRx.stats();
  JsonObject rx = o["rx"].to<JsonObject>();
  rx["enabled"] = sys.rfRx.enabled;
  rx["hw"] = sys.platform.rfRxHw;
  rx["frames"] = s.frames;
  rx["known"] = s.known;
  rx["unknown"] = s.unknown;
  rx["ignored"] = s.ignored;
  rx["disabled"] = s.disabled;
  rx["unsupported"] = s.unsupported;
  rx["timingMismatch"] = s.timingMismatch;
  rx["presses"] = s.presses;
  rx["rejected"] = s.rejected;
  rx["debounced"] = s.debounced;
  rx["echoes"] = s.echoes;
  rx["sensorRows"] = s.sensorRows;
  rx["quality"] = static_cast<int>(s.qualityAvg + 0.5f);
  rx["lastFrameAgeSec"] = s.frames ? elapsed(sys.clock().nowMs(), s.lastFrameMs) / 1000u : 0u;
  rx["isrEdges"] = s.isrEdges;
  rx["edgesPerSec"] = s.edgesPerSec;
  rx["droppedPulses"] = s.droppedPulses;
  rx["segments"] = sys.rfRx.segmenter().segments();
  rx["glitches"] = sys.rfRx.segmenter().glitches();
  rx["overflows"] = sys.rfRx.segmenter().overflows();
  JsonObject codec = rx["codec"].to<JsonObject>();
  codec["glitchUs"] = sys.rfRx.codec().glitchUs;
  codec["maxTimingError"] = sys.rfRx.codec().maxTimingError;
  JsonObject lm = o["learn"].to<JsonObject>();
  lm["active"] = sys.rfRx.learning();
  lm["remainingSec"] = sys.rfRx.learnRemainingMs() / 1000u;
  JsonObject tx = o["tx"].to<JsonObject>();
  const auto& tc = sys.rfTx.config();
  tx["enabled"] = tc.enabled;
  tx["hw"] = sys.platform.rfTxHw;
  tx["dutyCyclePct"] = tc.dutyCyclePct;
  tx["minIntervalMs"] = tc.minIntervalMs;
  tx["defaultTimeoutMs"] = tc.defaultTimeoutMs;
  tx["busy"] = sys.rfTx.busy();
  tx["queue"] = static_cast<int>(sys.rfTx.queueLength());
  tx["airtimeLastHourMs"] = sys.rfTx.airtimeLastHourMs();
  tx["budgetMs"] = sys.rfTx.budgetMs();
  const auto& ts = sys.rfTx.stats();
  tx["queued"] = ts.queued;
  tx["sent"] = ts.sent;
  tx["failed"] = ts.failed;
  tx["rejectedDuty"] = ts.rejectedDuty;
  tx["rejectedTimeout"] = ts.rejectedTimeout;
  tx["rejectedQueueFull"] = ts.rejectedQueueFull;
  tx["lastError"] = ts.lastError;
  JsonArray dec = o["decoders"].to<JsonArray>();
  for (auto& d : sys.rfSensors.decoders()) {
    JsonObject x = dec.add<JsonObject>();
    x["name"] = d.decoder->name();
    x["description"] = d.decoder->description();
    x["hardwareVerified"] = d.decoder->hardwareVerified();
    x["enabled"] = d.enabled;
    x["rows"] = d.rows;
    x["accepted"] = d.accepted;
  }
  JsonArray protos = o["protocols"].to<JsonArray>();
  for (auto pr : {"ev1527", "pt2262"}) protos.add(pr);
}

void monitorJson(HeatCoreSystem& sys, JsonObject o, uint32_t since) {
  JsonArray a = o["frames"].to<JsonArray>();
  for (auto& m : sys.rfRx.monitor(since, 64)) {
    JsonObject x = a.add<JsonObject>();
    x["seq"] = m.seq;
    x["epoch"] = m.epoch;
    x["ageMs"] = elapsed(sys.clock().nowMs(), m.ts);
    x["code"] = rf::formatCode(m.code, m.bits);
    x["bits"] = m.bits;
    x["pulseUs"] = m.pulseUs;
    x["quality"] = m.quality;
    x["status"] = rf::frameStatusName(m.status);
    if (m.pt2262) x["tristate"] = rf::triStateString(m.code, m.bits);
    if (m.device >= 0) {
      x["device"] = m.device;
      x["button"] = m.button;
      if (const rf::RxDevice* d = sys.rfReg.rxDevice(static_cast<uint16_t>(m.device))) x["deviceName"] = d->name;
    }
  }
  o["seq"] = sys.rfRx.monitorSeq();
}

void learnJson(HeatCoreSystem& sys, JsonObject o) {
  o["active"] = sys.rfRx.learning();
  o["remainingSec"] = sys.rfRx.learnRemainingMs() / 1000u;
  JsonArray c = o["candidates"].to<JsonArray>();
  auto list = sys.rfRx.candidates();
  std::sort(list.begin(), list.end(), [](const rf::LearnCandidate& a, const rf::LearnCandidate& b) { return a.lastMs > b.lastMs; });
  for (auto& x : list) {
    JsonObject j = c.add<JsonObject>();
    j["code"] = rf::formatCode(x.code, x.bits);
    j["bits"] = x.bits;
    j["pulseUs"] = static_cast<int>(x.pulseUs + 0.5f);
    j["quality"] = static_cast<int>(x.quality + 0.5f);
    j["frames"] = x.frames;
    j["presses"] = x.presses;
    j["reliable"] = x.reliable();
    j["ageSec"] = elapsed(sys.clock().nowMs(), x.lastMs) / 1000u;
    j["pt2262"] = x.pt2262;
    if (x.pt2262) j["tristate"] = rf::triStateString(x.code, x.bits);
    auto ev = rf::splitCode(rf::Protocol::Ev1527, x.code, x.bits);
    j["ev1527Address"] = rf::formatCode(ev.address, 20);
    j["ev1527Key"] = ev.data;
    if (x.knownDevice >= 0) {
      j["knownDevice"] = x.knownDevice;
      j["knownButton"] = x.knownButton;
    }
    j["ignored"] = sys.rfReg.isIgnored(x.code, x.bits);
  }
}

int rfRoutes(Ctx& c) {
  HeatCoreSystem& sys = c.sys;
  auto& p = c.p;
  JsonObject out = c.out.to<JsonObject>();
  if (p.size() == 2 && p[1] == "status" && c.is("GET")) {
    rfStatusJson(sys, out);
    return 200;
  }
  if (p.size() == 2 && p[1] == "config" && c.is("PUT")) {
    JsonObjectConst b = c.body();
    if (!b["rxEnabled"].isNull()) sys.rfRx.enabled = b["rxEnabled"] | true;
    JsonObjectConst codec = b["codec"];
    if (!codec.isNull()) {
      int g = codec["glitchUs"] | static_cast<int>(sys.rfRx.codec().glitchUs);
      float te = codec["maxTimingError"] | sys.rfRx.codec().maxTimingError;
      if (g < 20 || g > 200 || te < 0.2f || te > 0.6f) return c.fail(400, "Glitchfilter 20..200 us, Toleranz 0.2..0.6");
      sys.rfRx.codec().glitchUs = static_cast<uint16_t>(g);
      sys.rfRx.codec().maxTimingError = te;
    }
    JsonObjectConst tx = b["tx"];
    if (!tx.isNull()) {
      rf::TxConfig tc = sys.rfTx.config();
      tc.enabled = tx["enabled"] | tc.enabled;
      tc.dutyCyclePct = tx["dutyCyclePct"] | tc.dutyCyclePct;
      tc.minIntervalMs = tx["minIntervalMs"] | tc.minIntervalMs;
      tc.defaultTimeoutMs = tx["defaultTimeoutMs"] | tc.defaultTimeoutMs;
      if (tc.dutyCyclePct < 0.01f || tc.dutyCyclePct > 10.f) return c.fail(400, "Duty-Cycle 0.01..10 % (EN 300 220)");
      if (tc.minIntervalMs < 50 || tc.minIntervalMs > 60000) return c.fail(400, "Sendeabstand 50..60000 ms");
      if (tc.defaultTimeoutMs < 500 || tc.defaultTimeoutMs > 60000) return c.fail(400, "Timeout 500..60000 ms");
      sys.rfTx.config() = tc;
    }
    for (JsonPairConst kv : b["decoders"].as<JsonObjectConst>()) sys.rfSensors.setEnabled(kv.key().c_str(), kv.value() | false);
    sys.markDirty(ConfigSection::Rf);
    rfStatusJson(sys, out);
    return 200;
  }
  // ---- receive devices
  if (p.size() >= 2 && p[1] == "devices") {
    if (p.size() == 2 && c.is("GET")) {
      JsonArray a = out["devices"].to<JsonArray>();
      for (auto& d : sys.rfReg.rx) {
        JsonObject x = a.add<JsonObject>();
        rf::RfRegistry::rxDeviceToJson(d, x, true);
        x["state"] = d.state;
        x["available"] = d.available;
        x["lastSeenAgeSec"] = d.stats.seen ? elapsed(sys.clock().nowMs(), d.stats.lastSeenMs) / 1000u : 0u;
      }
      return 200;
    }
    if ((p.size() == 2 && c.is("POST")) || (p.size() == 3 && c.is("PUT"))) {
      rf::RxDevice d;
      std::string err = rf::RfRegistry::rxDeviceFromJson(d, c.body());
      if (!err.empty()) return c.fail(400, err);
      d.id = p.size() == 3 ? static_cast<uint16_t>(toInt(p[2])) : 0;
      if (d.function != rf::InputFunction::None && !sys.heating.room(d.room)) return c.fail(400, "Raum unbekannt");
      for (auto& b : d.buttons) {
        err = sys.validateBindings(b.bindings, EventSource::Rf);
        if (!err.empty()) return c.fail(400, b.name + ": " + err);
      }
      uint16_t id = 0;
      err = sys.rfReg.upsertRx(d, &id);
      if (!err.empty()) return c.fail(400, err);
      for (auto& b : d.buttons) sys.rfRx.forgetUnknown(b.code, d.bits);
      sys.markDirty(ConfigSection::Rf);
      sys.automation.revalidateAll();
      out["ok"] = true;
      out["id"] = id;
      return 200;
    }
    if (p.size() == 3 && c.is("DELETE")) {
      if (!sys.rfReg.removeRx(static_cast<uint16_t>(toInt(p[2])))) return c.fail(404, "Geraet nicht gefunden");
      sys.markDirty(ConfigSection::Rf);
      sys.automation.revalidateAll();
      out["ok"] = true;
      return 200;
    }
  }
  // ---- learn mode
  if (p.size() >= 2 && p[1] == "learn") {
    if (p.size() == 2 && c.is("GET")) {
      learnJson(sys, out);
      return 200;
    }
    if (p.size() == 3 && p[2] == "start" && c.is("POST")) {
      sys.rfRx.startLearn(c.body()["timeoutSec"] | 120u);
      learnJson(sys, out);
      return 200;
    }
    if (p.size() == 3 && p[2] == "stop" && c.is("POST")) {
      sys.rfRx.stopLearn();
      learnJson(sys, out);
      return 200;
    }
  }
  if (p.size() == 2 && p[1] == "monitor" && c.is("GET")) {
    monitorJson(sys, out, c.qu("since", 0));
    return 200;
  }
  if (p.size() == 2 && p[1] == "unknown") {
    if (c.is("DELETE")) {
      sys.rfRx.clearUnknown();
      out["ok"] = true;
      return 200;
    }
    JsonArray a = out["codes"].to<JsonArray>();
    for (auto& u : sys.rfRx.unknownCodes()) {
      JsonObject x = a.add<JsonObject>();
      x["code"] = rf::formatCode(u.code, u.bits);
      x["bits"] = u.bits;
      x["pulseUs"] = static_cast<int>(u.pulseUs + 0.5f);
      x["quality"] = static_cast<int>(u.quality + 0.5f);
      x["frames"] = u.frames;
      x["presses"] = u.presses;
      x["lastEpoch"] = u.lastEpoch;
      x["ageSec"] = elapsed(sys.clock().nowMs(), u.lastMs) / 1000u;
      if (u.pt2262) x["tristate"] = rf::triStateString(u.code, u.bits);
    }
    return 200;
  }
  if (p.size() >= 2 && p[1] == "ignored") {
    if (p.size() == 2 && c.is("GET")) {
      JsonArray a = out["ignored"].to<JsonArray>();
      for (auto& i : sys.rfReg.ignored) {
        JsonObject x = a.add<JsonObject>();
        x["code"] = rf::formatCode(i.code, i.bits);
        x["bits"] = i.bits;
        x["note"] = i.note;
      }
      return 200;
    }
    if (p.size() == 2 && c.is("POST")) {
      uint32_t code = 0;
      if (!rf::parseCode(c.body()["code"] | "", code)) return c.fail(400, "Code ungueltig");
      uint8_t bits = c.body()["bits"] | 24;
      std::string err = sys.rfReg.addIgnored(code, bits, c.body()["note"] | "");
      if (err.empty()) {
        sys.rfRx.forgetUnknown(code, bits);
        sys.markDirty(ConfigSection::Rf);
      }
      return c.okOrFail(err);
    }
    if (p.size() == 3 && c.is("DELETE")) {
      uint32_t code = 0;
      if (!rf::parseCode(p[2].c_str(), code)) return c.fail(400, "Code ungueltig");
      if (!sys.rfReg.removeIgnored(code, 24)) return c.fail(404, "Nicht auf der Ignorierliste");
      sys.markDirty(ConfigSection::Rf);
      out["ok"] = true;
      return 200;
    }
  }
  if (p.size() >= 2 && p[1] == "raw") {
    if (p.size() == 3 && p[2] == "capture" && c.is("POST")) {
      uint32_t n = c.body()["count"] | 4u;
      sys.rfRx.startRawCapture(static_cast<uint8_t>(n > 6 ? 6 : n));
      out["ok"] = true;
      return 200;
    }
    JsonArray a = out["captures"].to<JsonArray>();
    for (auto& r : sys.rfRx.rawCaptures()) {
      JsonObject x = a.add<JsonObject>();
      x["ageMs"] = elapsed(sys.clock().nowMs(), r.ts);
      x["leadGapUs"] = r.leadGapUs;
      x["decoded"] = r.decoded;
      JsonArray pa = x["pulses"].to<JsonArray>();
      for (auto& pu : r.pulses) pa.add(pu.level ? static_cast<int>(pu.us) : -static_cast<int>(pu.us));
    }
    out["pending"] = sys.rfRx.rawCaptureRemaining();
    return 200;
  }
  // ---- transmitter
  if (p.size() >= 2 && p[1] == "tx") {
    if (p.size() == 3 && p[2] == "devices" && c.is("GET")) {
      JsonArray a = out["devices"].to<JsonArray>();
      for (auto& d : sys.rfReg.tx) rf::RfRegistry::txDeviceToJson(d, a.add<JsonObject>());
      JsonArray g = out["groups"].to<JsonArray>();
      for (auto& x : sys.rfReg.groups()) g.add(x);
      return 200;
    }
    if (p.size() >= 3 && p[2] == "devices" && ((p.size() == 3 && c.is("POST")) || (p.size() == 4 && c.is("PUT")))) {
      rf::TxDevice d;
      std::string err = rf::RfRegistry::txDeviceFromJson(d, c.body());
      if (!err.empty()) return c.fail(400, err);
      d.id = p.size() == 4 ? static_cast<uint16_t>(toInt(p[3])) : 0;
      uint16_t id = 0;
      err = sys.rfReg.upsertTx(d, &id);
      if (!err.empty()) return c.fail(400, err);
      sys.markDirty(ConfigSection::Rf);
      sys.automation.revalidateAll();
      out["ok"] = true;
      out["id"] = id;
      return 200;
    }
    if (p.size() == 4 && p[2] == "devices" && c.is("DELETE")) {
      if (!sys.rfReg.removeTx(static_cast<uint16_t>(toInt(p[3])))) return c.fail(404, "Sender nicht gefunden");
      sys.markDirty(ConfigSection::Rf);
      sys.automation.revalidateAll();
      out["ok"] = true;
      return 200;
    }
    if (p.size() == 3 && p[2] == "send" && c.is("POST")) {
      JsonObjectConst b = c.body();
      std::string err;
      if (!b["command"].isNull()) {
        err = sys.rfTx.enqueue(b["command"] | "", EventSource::Web, 0, 0, false);
      } else {
        uint32_t code = 0;
        if (!rf::parseCode(b["code"] | "", code)) return c.fail(400, "Code ungueltig");
        err = sys.rfTx.enqueueRaw(code, 24, b["pulseUs"] | 350, b["repeats"] | 8, "Testsendung", EventSource::Web, false);
      }
      if (err.empty()) {
        out["estimatedAirtimeMs"] = rf::RfTransmitter::estimateAirtimeMs(24, b["pulseUs"] | 350, b["repeats"] | 8);
      }
      return c.okOrFail(err);
    }
    if (p.size() == 3 && p[2] == "history" && c.is("GET")) {
      JsonArray a = out["history"].to<JsonArray>();
      for (auto& h : sys.rfTx.history()) {
        JsonObject x = a.add<JsonObject>();
        x["job"] = h.jobId;
        x["epoch"] = h.epoch;
        x["ok"] = h.ok;
        x["airtimeMs"] = h.airtimeMs;
        x["label"] = h.label;
        x["reason"] = h.reason;
        x["origin"] = eventSourceName(h.origin);
      }
      return 200;
    }
    if (p.size() == 3 && p[2] == "loopback" && c.is("POST")) return c.okOrFail(sys.startRfLoopback());
  }
  if (p.size() == 2 && p[1] == "sensors" && c.is("GET")) {
    JsonArray d = out["discovered"].to<JsonArray>();
    for (auto& x : sys.sensors.discovered()) {
      if (x.kind != SensorKind::Rf && x.kind != SensorKind::External) continue;
      JsonObject o = d.add<JsonObject>();
      o["id"] = x.id;
      o["value"] = x.lastValue;
      if (x.hasHumidity) o["humidity"] = x.humidity;
      if (x.batteryOk >= 0) o["batteryOk"] = x.batteryOk == 1;
      o["seen"] = x.seen;
      o["ageSec"] = elapsed(sys.clock().nowMs(), x.lastMs) / 1000u;
    }
    JsonArray s = out["sensors"].to<JsonArray>();
    JsonDocument all;
    sys.sensors.toJson(all.to<JsonArray>(), true);
    for (JsonObject x : all.as<JsonArray>()) {
      const char* kind = x["kind"] | "";
      if (!std::strcmp(kind, "rf") || !std::strcmp(kind, "external")) s.add(x);
    }
    return 200;
  }
  if (p.size() == 2 && p[1] == "rtl433" && c.is("POST")) {
    std::string line = c.req.body;
    rf::SensorReading r;
    std::string err;
    if (!rf::parseRtl433Json(line.c_str(), r, err)) return c.fail(400, err);
    sys.rfSensors.injectExternal(r, sys.clock().nowMs());
    out["ok"] = true;
    out["key"] = r.key();
    return 200;
  }
  return c.fail(404, "unbekannter Funk-Endpunkt");
}

// -------------------------------------------------------------- automation

void catalogJson(HeatCoreSystem& sys, JsonObject o) {
  auto field = [](JsonArray f, const char* key, const char* label, const char* kind) {
    JsonObject x = f.add<JsonObject>();
    x["key"] = key;
    x["label"] = label;
    x["kind"] = kind;
    return x;
  };
  auto entry = [](JsonArray arr, const char* type, const char* label) {
    JsonObject x = arr.add<JsonObject>();
    x["type"] = type;
    x["label"] = label;
    return x["fields"].to<JsonArray>();
  };
  JsonArray t = o["triggers"].to<JsonArray>();
  {
    JsonArray f = entry(t, "input", "Eingang (Draht oder Funk)");
    field(f, "ref", "Eingang", "input");
    field(f, "gesture", "Ereignis", "gesture");
  }
  {
    JsonArray f = entry(t, "temp_threshold", "Temperaturgrenzwert");
    field(f, "ref", "Quelle", "temp_ref");
    field(f, "direction", "Richtung", "direction");
    field(f, "value", "Grenzwert C", "number");
    field(f, "hysteresis", "Hysterese K", "number");
  }
  {
    JsonArray f = entry(t, "temp_change", "Temperaturaenderung");
    field(f, "ref", "Quelle", "temp_ref");
    field(f, "delta", "Aenderung K (+/-)", "number");
    field(f, "windowSec", "Zeitfenster s", "number");
  }
  {
    JsonArray f = entry(t, "room_state", "Raumzustand");
    field(f, "ref", "Raum", "room_any");
    field(f, "state", "Zustand", "room_state");
  }
  {
    JsonArray f = entry(t, "valve_state", "Ventilzustand");
    field(f, "circuit", "Heizkreis (0 = alle)", "number");
    field(f, "state", "Zustand", "valve_state");
  }
  {
    JsonArray f = entry(t, "time", "Zeitereignis");
    field(f, "time", "Uhrzeit", "time");
    field(f, "days", "Wochentage", "days");
  }
  {
    JsonArray f = entry(t, "mqtt", "MQTT-Nachricht (<basis>/in/<thema>)");
    field(f, "topic", "Thema", "text");
    field(f, "payload", "Inhalt (leer = beliebig)", "text");
  }
  field(entry(t, "sensor_fault", "Sensorfehler"), "ref", "Sensor (* = alle)", "sensor_any");
  {
    JsonArray f = entry(t, "model_event", "Lernmodell-Ereignis");
    field(f, "ref", "Raum", "room_any");
    field(f, "state", "Ereignis", "model_state");
  }
  entry(t, "manual", "Manuell (Webinterface / Home Assistant)");
  field(entry(t, "global_mode", "Betriebsart geaendert"), "state", "Betriebsart", "global_mode_any");
  field(entry(t, "alarm", "Alarm"), "ref", "Code-Praefix (* = alle)", "text");

  JsonArray c = o["conditions"].to<JsonArray>();
  {
    JsonArray f = entry(c, "temperature", "Aktuelle Temperatur");
    field(f, "ref", "Quelle", "temp_ref");
    field(f, "op", "Vergleich", "op");
    field(f, "value", "Wert C", "number");
  }
  {
    JsonArray f = entry(c, "setpoint", "Solltemperatur");
    field(f, "ref", "Raum", "room");
    field(f, "op", "Vergleich", "op");
    field(f, "value", "Wert C", "number");
  }
  {
    JsonArray f = entry(c, "time_range", "Zeitbereich");
    field(f, "from", "von", "time");
    field(f, "to", "bis", "time");
  }
  field(entry(c, "weekday", "Wochentag"), "days", "Tage", "days");
  field(entry(c, "global_mode", "Betriebsart"), "mode", "Betriebsart", "global_mode");
  {
    JsonArray f = entry(c, "room_mode", "Raum-Betriebsart");
    field(f, "ref", "Raum", "room");
    field(f, "mode", "Modus", "room_mode");
  }
  {
    JsonArray f = entry(c, "presence", "Anwesenheit");
    field(f, "ref", "Raum (leer = Haus)", "room_opt");
    field(f, "value", "anwesend", "bool");
  }
  {
    JsonArray f = entry(c, "window", "Fensterstatus");
    field(f, "ref", "Raum", "room");
    field(f, "value", "offen", "bool");
  }
  {
    JsonArray f = entry(c, "sensor_available", "Sensorverfuegbarkeit");
    field(f, "ref", "Sensor", "sensor");
    field(f, "value", "verfuegbar", "bool");
  }
  {
    JsonArray f = entry(c, "input_state", "Zustand Eingang");
    field(f, "ref", "Eingang", "input_state_ref");
    field(f, "value", "aktiv/offen", "bool");
  }
  {
    JsonArray f = entry(c, "output_state", "Zustand Ausgang");
    field(f, "output", "Ausgang", "number");
    field(f, "value", "an", "bool");
  }
  {
    JsonArray f = entry(c, "room_group", "Raumgruppe");
    field(f, "ref", "Gruppe", "group");
    field(f, "state", "Zustand", "group_state");
  }

  JsonArray a = o["actions"].to<JsonArray>();
  {
    JsonArray f = entry(a, "set_setpoint", "Raum-Solltemperatur setzen");
    field(f, "room", "Raum", "room");
    field(f, "value", "Sollwert C", "number");
  }
  {
    JsonArray f = entry(a, "adjust_setpoint", "Solltemperatur erhoehen/verringern");
    field(f, "room", "Raum", "room");
    field(f, "delta", "Aenderung K", "number");
  }
  {
    JsonArray f = entry(a, "set_room_mode", "Raum-Betriebsart");
    field(f, "room", "Raum", "room");
    field(f, "mode", "Modus", "room_mode");
  }
  {
    JsonArray f = entry(a, "set_global_mode", "Betriebsmodus wechseln");
    field(f, "mode", "Betriebsart", "global_mode");
    field(f, "durationSec", "Dauer s (0 = offen)", "number");
  }
  {
    JsonArray f = entry(a, "activate_room_group", "Raumgruppe aktivieren");
    field(f, "group", "Gruppe", "group");
    field(f, "mode", "Modus", "room_mode");
  }
  {
    JsonArray f = entry(a, "set_output", "Nicht sicherheitskritischen Ausgang schalten");
    field(f, "output", "Ausgang", "general_output");
    field(f, "state", "an", "bool");
    field(f, "durationSec", "Dauer s", "number");
  }
  field(entry(a, "rf_send", "Funkbefehl senden"), "command", "Befehl", "rf_command");
  {
    JsonArray f = entry(a, "mqtt_publish", "MQTT veroeffentlichen (<basis>/out/<thema>)");
    field(f, "topic", "Thema", "text");
    field(f, "payload", "Inhalt", "text");
  }
  {
    JsonArray f = entry(a, "notify", "Benachrichtigung");
    field(f, "message", "Text", "text");
    field(f, "level", "Stufe 0..2", "number");
  }
  field(entry(a, "display_page", "Displayseite wechseln"), "page", "Seite 1..5", "number");
  entry(a, "display_next", "Display: naechste Seite");
  entry(a, "display_prev", "Display: vorherige Seite");
  entry(a, "display_wake", "Display einschalten");
  entry(a, "display_room_next", "Display: naechster Raum");
  field(entry(a, "displayed_room_adjust", "Sollwert des angezeigten Raums"), "delta", "Aenderung K", "number");
  {
    JsonArray f = entry(a, "raise_alarm", "Alarm registrieren");
    field(f, "code", "Code (user.*)", "text");
    field(f, "message", "Text", "text");
    field(f, "level", "Stufe 0..2", "number");
  }
  field(entry(a, "clear_alarm", "Alarm loeschen"), "code", "Code (user.*)", "text");
  {
    JsonArray f = entry(a, "window_state", "Fensterstatus setzen");
    field(f, "room", "Raum", "room");
    field(f, "open", "offen", "bool");
  }
  {
    JsonArray f = entry(a, "presence", "Anwesenheit setzen");
    field(f, "room", "Raum (leer = alle)", "room_opt");
    field(f, "present", "anwesend", "bool");
  }
  {
    JsonArray f = entry(a, "heat_demand", "Waermeanforderung");
    field(f, "room", "Raum", "room");
    field(f, "on", "an", "bool");
    field(f, "durationSec", "Dauer s (max 4 h)", "number");
  }

  // reference lists for the editor
  JsonObject refs = o["refs"].to<JsonObject>();
  JsonArray rooms = refs["rooms"].to<JsonArray>();
  for (auto& r : sys.heating.rooms) {
    JsonObject x = rooms.add<JsonObject>();
    x["id"] = r.id;
    x["name"] = r.name;
  }
  JsonArray groups = refs["groups"].to<JsonArray>();
  for (auto& g : sys.heating.groups()) groups.add(g);
  JsonArray sensors = refs["sensors"].to<JsonArray>();
  for (auto& s : sys.sensors.sensors()) {
    JsonObject x = sensors.add<JsonObject>();
    x["id"] = s.id;
    x["name"] = s.name;
  }
  JsonArray inputs = refs["inputs"].to<JsonArray>();
  for (int i = 0; i < kNumInputs; ++i) {
    if (sys.io.inCfg[static_cast<size_t>(i)].kind == InputKind::Disabled) continue;
    JsonObject x = inputs.add<JsonObject>();
    x["ref"] = "di:" + std::to_string(i + 1);
    x["name"] = "DI" + std::to_string(i + 1) + " " + sys.io.inCfg[static_cast<size_t>(i)].name;
  }
  for (auto& d : sys.rfReg.rx) {
    JsonObject x = inputs.add<JsonObject>();
    x["ref"] = "rf:" + std::to_string(d.id);
    x["name"] = "Funk " + d.name + " (alle Tasten)";
    for (auto& b : d.buttons) {
      JsonObject y = inputs.add<JsonObject>();
      y["ref"] = "rf:" + std::to_string(d.id) + "/" + std::to_string(b.index);
      y["name"] = "Funk " + d.name + " / " + b.name;
    }
  }
  JsonArray outs = refs["outputs"].to<JsonArray>();
  for (int i = 0; i < kNumOutputs; ++i) {
    const OutputConfig& oc = sys.io.outCfg[static_cast<size_t>(i)];
    JsonObject x = outs.add<JsonObject>();
    x["n"] = i + 1;
    x["name"] = oc.name;
    x["function"] = outputFunctionName(oc.function);
    x["automationAllowed"] = oc.automationAllowed;
    x["haAllowed"] = oc.haAllowed;
  }
  JsonArray cmds = refs["rfCommands"].to<JsonArray>();
  for (auto& d : sys.rfReg.tx)
    for (auto& cmd : d.commands) {
      JsonObject x = cmds.add<JsonObject>();
      x["ref"] = std::to_string(d.id) + "/" + std::to_string(cmd.id);
      x["name"] = d.name + " / " + cmd.name;
      x["automationAllowed"] = cmd.automationAllowed;
    }
  for (auto& g : sys.rfReg.groups()) {
    JsonObject x = cmds.add<JsonObject>();
    x["ref"] = "group:" + g + "/<befehlsname>";
    x["name"] = "Gruppe " + g;
  }
  JsonObject en = o["enums"].to<JsonObject>();
  auto add = [](JsonArray arr, std::initializer_list<const char*> v) {
    for (auto s : v) arr.add(s);
  };
  add(en["gesture"].to<JsonArray>(), {"any", "press", "release", "short", "long", "open", "closed", "unavailable", "available"});
  add(en["room_state"].to<JsonArray>(), {"setpoint", "mode", "window_open", "window_closed", "heating_on", "heating_off",
                                         "presence_on", "presence_off"});
  add(en["valve_state"].to<JsonArray>(), {"open", "closed", "fault"});
  add(en["model_state"].to<JsonArray>(), {"ready", "degraded", "learning", "fault_suspected"});
  add(en["room_mode"].to<JsonArray>(), {"auto", "comfort", "eco", "frost", "off"});
  add(en["global_mode"].to<JsonArray>(), {"normal", "eco", "away", "vacation"});
  add(en["op"].to<JsonArray>(), {"<", "<=", "==", ">=", ">"});
  add(en["direction"].to<JsonArray>(), {"above", "below"});
  add(en["group_state"].to<JsonArray>(), {"any_heating", "none_heating", "all_window_closed"});
  add(en["input_function"].to<JsonArray>(), {"none", "window", "presence", "heat_demand"});
  add(en["button_role"].to<JsonArray>(), {"button", "state_on", "state_off"});
}

void ruleTestJson(const RuleTestReport& rep, JsonObject o) {
  o["wouldRun"] = rep.wouldRun;
  if (rep.triggerTested) o["triggerMatched"] = rep.triggerMatched;
  JsonArray c = o["conditions"].to<JsonArray>();
  for (auto& x : rep.conditions) {
    JsonObject j = c.add<JsonObject>();
    j["text"] = x.first;
    j["ok"] = x.second;
  }
  JsonArray a = o["actions"].to<JsonArray>();
  for (auto& x : rep.actions) {
    JsonObject j = a.add<JsonObject>();
    j["text"] = x.first;
    j["result"] = x.second;
  }
  JsonArray p = o["problems"].to<JsonArray>();
  for (auto& x : rep.problems) p.add(x);
}

bool eventFromJson(JsonObjectConst o, Event& e) {
  if (o.isNull()) return false;
  e = Event(eventTypeFromName(o["type"] | "manual"), EventSource::Web);
  if (e.type == EventType::None) return false;
  e.gesture = gestureFromName(o["gesture"] | "none");
  e.setRef(o["ref"] | "");
  e.setText(o["text"] | "");
  e.a = o["a"] | -1;
  e.b = o["b"] | -1;
  e.value = o["value"] | 0.f;
  return true;
}

int ruleRoutes(Ctx& c) {
  HeatCoreSystem& sys = c.sys;
  auto& p = c.p;
  JsonObject out = c.out.to<JsonObject>();
  if (p.size() == 1 && c.is("GET")) {
    sys.automation.toJson(out["rules"].to<JsonArray>(), true);
    JsonArray d = out["descriptions"].to<JsonArray>();
    for (auto& r : sys.automation.rules()) {
      JsonObject x = d.add<JsonObject>();
      x["id"] = r.id;
      JsonArray t = x["triggers"].to<JsonArray>();
      for (auto& tr : r.triggers) t.add(sys.automation.describeTrigger(tr));
      JsonArray cs = x["conditions"].to<JsonArray>();
      for (auto& cd : r.conditions) cs.add(sys.automation.describeCondition(cd));
      JsonArray as = x["actions"].to<JsonArray>();
      for (auto& ac : r.actions) as.add(AutomationEngine::describeAction(ac));
    }
    const auto& st = sys.automation.stats();
    JsonObject s = out["stats"].to<JsonObject>();
    s["executions"] = st.executions;
    s["blockedDepth"] = st.blockedDepth;
    s["blockedSelf"] = st.blockedSelf;
    s["blockedRate"] = st.blockedRate;
    s["conflicts"] = st.conflicts;
    s["storms"] = st.storms;
    s["actionErrors"] = st.actionErrors;
    s["paused"] = sys.automation.paused();
    s["maxDepth"] = sys.automation.maxDepth;
    return 200;
  }
  if (p.size() == 2 && p[1] == "validate" && c.is("POST")) {
    Rule r;
    std::string err;
    if (!ruleFromJson(r, c.body(), err)) return c.fail(400, err);
    err = sys.automation.validate(r);
    out["valid"] = err.empty();
    out["error"] = err;
    return 200;
  }
  if ((p.size() == 1 && c.is("POST")) || (p.size() == 2 && c.is("PUT"))) {
    Rule r;
    std::string err;
    if (!ruleFromJson(r, c.body(), err)) return c.fail(400, err);
    r.id = p.size() == 2 ? static_cast<uint16_t>(toInt(p[1])) : 0;
    uint16_t id = 0;
    std::string verr;
    err = sys.automation.upsert(r, &id, &verr);
    if (!err.empty()) return c.fail(400, err);
    sys.markDirty(ConfigSection::Rules);
    out["ok"] = true;
    out["id"] = id;
    out["enabled"] = sys.automation.rule(id)->enabled;
    out["validationError"] = verr;
    return 200;
  }
  if (p.size() == 2 && c.is("DELETE")) {
    if (!sys.automation.remove(static_cast<uint16_t>(toInt(p[1])))) return c.fail(404, "Regel nicht gefunden");
    sys.markDirty(ConfigSection::Rules);
    out["ok"] = true;
    return 200;
  }
  if (p.size() == 3 && c.is("POST")) {
    uint16_t id = static_cast<uint16_t>(toInt(p[1]));
    if (p[2] == "enable") {
      std::string err;
      bool en = c.body()["enabled"] | true;
      if (!sys.automation.setEnabled(id, en, &err)) return c.fail(400, err);
      sys.markDirty(ConfigSection::Rules);
      out["ok"] = true;
      return 200;
    }
    if (p[2] == "test") {
      Event e;
      bool have = eventFromJson(c.body()["event"], e);
      RuleTestReport rep = sys.automation.test(id, have ? &e : nullptr);
      ruleTestJson(rep, out);
      return 200;
    }
    if (p[2] == "run") return c.okOrFail(sys.runRule(id, EventSource::Web));
  }
  return c.fail(404, "unbekannter Regel-Endpunkt");
}

// ----------------------------------------------------------------- misc

bool parseSection(const std::string& s, ConfigSection& out) { return configSectionFromName(s.c_str(), out); }

}  // namespace

std::string ApiRouter::base64(const uint8_t* data, size_t len) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t v = static_cast<uint32_t>(data[i]) << 16;
    if (i + 1 < len) v |= static_cast<uint32_t>(data[i + 1]) << 8;
    if (i + 2 < len) v |= data[i + 2];
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += i + 1 < len ? tbl[(v >> 6) & 63] : '=';
    out += i + 2 < len ? tbl[v & 63] : '=';
  }
  return out;
}

ApiResponse ApiRouter::handle(const ApiRequest& req) {
  Ctx c(sys_, req);
  auto segs = splitPath(req.path);
  if (segs.empty() || segs[0] != "api") {
    c.fail(404, "kein API-Pfad");
  } else {
    c.p.assign(segs.begin() + 1, segs.end());
    if (!req.body.empty() && (req.method == "POST" || req.method == "PUT")) {
      DeserializationError e = deserializeJson(c.in, req.body);
      if (e && !(c.p.size() == 2 && c.p[0] == "rf" && c.p[1] == "rtl433")) c.fail(400, std::string("JSON: ") + e.c_str());
    }
    if (c.status == 200) {
      auto& p = c.p;
      HeatCoreSystem& sys = sys_;
      JsonObject out = c.out.to<JsonObject>();
      std::string head = p.empty() ? "" : p[0];
      if (head == "status" && c.is("GET")) {
        statusJson(sys, out);
      } else if (head == "alarms" && c.is("GET")) {
        sys.alarms.toJson(out["alarms"].to<JsonArray>());
      } else if (head == "events" && c.is("GET")) {
        JsonArray a = out["events"].to<JsonArray>();
        for (auto& e : sys.bus.history(c.qu("since", 0), 100)) {
          JsonObject x = a.add<JsonObject>();
          x["seq"] = e.seq;
          x["type"] = eventTypeName(e.type);
          x["source"] = eventSourceName(e.source);
          if (e.gesture != Gesture::None) x["gesture"] = gestureName(e.gesture);
          x["ageMs"] = elapsed(sys.clock().nowMs(), e.ts);
          if (e.ref[0]) x["ref"] = e.ref;
          if (e.text[0]) x["text"] = e.text;
          if (e.a >= 0) x["a"] = e.a;
          if (e.b >= 0) x["b"] = e.b;
          x["value"] = e.value;
          if (e.depth) x["depth"] = e.depth;
          if (e.ruleId) x["rule"] = e.ruleId;
        }
        out["seq"] = sys.bus.lastSeq();
        out["dropped"] = sys.bus.dropped();
      } else if (head == "hardware") {
        if (p.size() == 2 && p[1] == "selftest" && c.is("POST")) {
          std::string r = sys.platformCommand ? sys.platformCommand("selftest", JsonObjectConst()) : "nicht verfuegbar";
          c.okOrFail(r);
        } else if (p.size() == 1 && c.is("GET")) {
          hardwareJson(sys, out);
        } else {
          c.fail(404, "?");
        }
      } else if (head == "io") {
        if (c.is("GET")) {
          sys.io.toJson(out, true);
        } else if (c.is("PUT")) {
          c.okOrFail(sys.sectionFromJson(ConfigSection::Io, c.body()));
        } else {
          c.fail(405, "Methode nicht erlaubt");
        }
      } else if (head == "outputs" && p.size() == 3) {
        int n = toInt(p[1]);
        if (p[2] == "manual" && c.is("POST")) {
          c.okOrFail(sys.manualOutput(n, c.body()["state"] | false, c.body()["durationSec"] | 600u, EventSource::Web));
        } else if (p[2] == "manual" && c.is("DELETE")) {
          sys.clearManualOutput(n);
          out["ok"] = true;
        } else if (p[2] == "request" && c.is("POST")) {
          c.okOrFail(sys.requestOutput(n, c.body()["state"] | false, c.body()["durationSec"] | 0u, EventSource::Web));
        } else {
          c.fail(404, "?");
        }
      } else if (head == "heating") {
        if (c.is("GET")) {
          sys.heating.toJson(out, true);
        } else if (c.is("PUT")) {
          c.okOrFail(sys.sectionFromJson(ConfigSection::Heating, c.body()));
        } else {
          c.fail(405, "Methode nicht erlaubt");
        }
      } else if (head == "rooms" && p.size() >= 3) {
        Room* r = sys.heating.room(p[1]);
        ActionOrigin web{EventSource::Web, 0, 0, false};
        if (!r) {
          c.fail(404, "Raum nicht gefunden");
        } else if (p[2] == "setpoint" && c.is("POST")) {
          Action a{ActionType::SetSetpoint, r->id, "", "", c.body()["value"] | 0.f};
          c.okOrFail(sys.execute(a, web));
        } else if (p[2] == "adjust" && c.is("POST")) {
          Action a{ActionType::AdjustSetpoint, r->id, "", "", c.body()["delta"] | 0.f};
          c.okOrFail(sys.execute(a, web));
        } else if (p[2] == "mode" && c.is("POST")) {
          Action a{ActionType::SetRoomMode, r->id, "", c.body()["mode"] | ""};
          c.okOrFail(sys.execute(a, web));
        } else if (p[2] == "manual" && c.is("DELETE")) {
          r->manualActive = false;
          out["ok"] = true;
        } else {
          c.fail(404, "?");
        }
      } else if (head == "mode" && c.is("POST")) {
        Action a{ActionType::SetGlobalMode, "", "", c.body()["mode"] | ""};
        a.durationSec = c.body()["durationSec"] | 0u;
        c.okOrFail(sys.execute(a, ActionOrigin{EventSource::Web, 0, 0, false}));
      } else if (head == "sensors") {
        if (p.size() == 1 && c.is("GET")) {
          sys.sensors.toJson(out["sensors"].to<JsonArray>(), true);
          JsonArray d = out["discovered"].to<JsonArray>();
          for (auto& x : sys.sensors.discovered()) {
            JsonObject o = d.add<JsonObject>();
            o["id"] = x.id;
            o["kind"] = sensorKindName(x.kind);
            o["value"] = x.lastValue;
            o["seen"] = x.seen;
          }
          out["autoAdoptOneWire"] = sys.sensors.autoAdoptOneWire;
        } else if (p.size() == 2 && p[1] == "adopt" && c.is("POST")) {
          std::string err = sys.sensors.adopt(c.body()["id"] | "", c.body()["name"] | "", c.body()["room"] | "",
                                              sensorRoleFromName(c.body()["role"] | "room"));
          if (err.empty()) sys.markDirty(ConfigSection::Sensors);
          c.okOrFail(err);
        } else if (p.size() == 2 && c.is("PUT")) {
          Sensor s;
          JsonObjectConst b = c.body();
          s.id = p[1];
          s.name = b["name"] | s.id.c_str();
          s.role = sensorRoleFromName(b["role"] | "room");
          s.room = b["room"] | "";
          s.offset = b["offset"] | 0.f;
          s.timeoutSec = b["timeoutSec"] | 300u;
          s.priority = b["priority"] | 50;
          s.fallbackId = b["fallbackId"] | "";
          s.enabled = b["enabled"] | true;
          s.minValid = b["minValid"] | -40.f;
          s.maxValid = b["maxValid"] | 100.f;
          std::string err = sys.sensors.upsert(s);
          if (err.empty()) {
            sys.markDirty(ConfigSection::Sensors);
            sys.automation.revalidateAll();
          }
          c.okOrFail(err);
        } else if (p.size() == 2 && c.is("DELETE")) {
          if (!sys.sensors.remove(p[1])) c.fail(404, "Sensor nicht gefunden");
          else {
            sys.markDirty(ConfigSection::Sensors);
            sys.automation.revalidateAll();
            out["ok"] = true;
          }
        } else if (p.size() == 1 && c.is("PUT")) {
          sys.sensors.autoAdoptOneWire = c.body()["autoAdoptOneWire"] | false;
          sys.markDirty(ConfigSection::Sensors);
          out["ok"] = true;
        } else {
          c.fail(404, "?");
        }
      } else if (head == "safety") {
        if (c.is("GET")) sys.safety.toJson(out, true);
        else if (c.is("PUT")) c.okOrFail(sys.sectionFromJson(ConfigSection::Safety, c.body()));
        else c.fail(405, "Methode nicht erlaubt");
      } else if (head == "rf") {
        rfRoutes(c);
      } else if (head == "display") {
        if (p.size() == 1 && c.is("GET")) {
          sys.display.toJson(out, true);
          JsonArray r = out["availableRooms"].to<JsonArray>();
          for (auto& x : sys.heating.rooms) {
            JsonObject o = r.add<JsonObject>();
            o["id"] = x.id;
            o["name"] = x.name;
          }
        } else if (p.size() == 2 && p[1] == "config" && c.is("PUT")) {
          c.okOrFail(sys.sectionFromJson(ConfigSection::Display, c.body()));
        } else if (p.size() == 2 && p[1] == "framebuffer" && c.is("GET")) {
          int page = static_cast<int>(c.qu("page", 0));
          DisplaySnapshot snap;
          sys.buildDisplaySnapshot(snap);
          Framebuffer fb;
          if (page >= 1 && page <= kNumPages) sys.display.renderPage(static_cast<DisplayPage>(page - 1), fb, snap);
          else sys.display.render(fb, snap);
          if (sys.display.cfg.rotation == 180) fb.rotate180();
          out["page"] = page ? page : sys.display.page() + 1;
          out["screenOn"] = sys.display.screenOn();
          out["rotation"] = sys.display.cfg.rotation;
          out["width"] = Framebuffer::W;
          out["height"] = Framebuffer::H;
          out["fb"] = base64(fb.data(), Framebuffer::kBytes);
        } else if (p.size() == 2 && p[1] == "page" && c.is("POST")) {
          std::string act = c.body()["action"] | "";
          Action a;
          if (act == "next") a.type = ActionType::DisplayNext;
          else if (act == "prev") a.type = ActionType::DisplayPrev;
          else if (act == "wake") a.type = ActionType::DisplayWake;
          else if (act == "room_next") a.type = ActionType::DisplayRoomNext;
          else {
            a.type = ActionType::DisplayPage;
            a.index = c.body()["page"] | 0;
          }
          c.okOrFail(sys.execute(a, ActionOrigin{EventSource::Web, 0, 0, false}));
        } else {
          c.fail(404, "?");
        }
      } else if (head == "rules") {
        ruleRoutes(c);
      } else if (head == "automation") {
        if (p.size() == 2 && p[1] == "catalog" && c.is("GET")) {
          catalogJson(sys, out);
        } else if (p.size() == 2 && p[1] == "log" && c.is("GET")) {
          JsonArray a = out["log"].to<JsonArray>();
          for (auto& l : sys.automation.log(c.qu("since", 0), 60)) {
            JsonObject x = a.add<JsonObject>();
            x["seq"] = l.seq;
            x["epoch"] = l.epoch;
            x["ageMs"] = elapsed(sys.clock().nowMs(), l.ms);
            x["rule"] = l.rule;
            x["ruleId"] = l.ruleId;
            x["trigger"] = l.trigger;
            x["result"] = l.result;
            x["dryRun"] = l.dryRun;
          }
        } else {
          c.fail(404, "?");
        }
      } else if (head == "model") {
        if (p.size() == 1 && c.is("GET")) {
          sys.model.toJson(out, true);
        } else if (p.size() == 2 && p[1] == "config" && c.is("PUT")) {
          JsonDocument tmp;
          sys.model.toJson(tmp.to<JsonObject>(), false);
          tmp["config"] = c.body()["config"];
          if (!c.body()["enabled"].isNull()) tmp["enabled"] = c.body()["enabled"];
          sys.model.fromJson(tmp.as<JsonObjectConst>());
          sys.markDirty(ConfigSection::Model);
          out["ok"] = true;
        } else if (p.size() == 3 && p[2] == "reset" && c.is("POST")) {
          if (!sys.heating.room(p[1])) c.fail(404, "Raum nicht gefunden");
          else {
            sys.model.reset(p[1]);
            sys.markDirty(ConfigSection::Model);
            out["ok"] = true;
          }
        } else {
          c.fail(404, "?");
        }
      } else if (head == "config") {
        ConfigSection s;
        if (p.size() == 2 && p[1] == "export" && c.is("GET")) {
          for (int i = 0; i < static_cast<int>(ConfigSection::Count); ++i) {
            auto sec = static_cast<ConfigSection>(i);
            sys.sectionToJson(sec, out[configSectionName(sec)].to<JsonObject>(), false);
          }
        } else if (p.size() == 2 && p[1] == "import" && c.is("POST")) {
          // dependency order: rooms before inputs/RF bindings and rules
          const ConfigSection order[] = {ConfigSection::System, ConfigSection::Network, ConfigSection::Mqtt,
                                         ConfigSection::Safety, ConfigSection::Heating, ConfigSection::Sensors,
                                         ConfigSection::Io,     ConfigSection::Rf,      ConfigSection::Display,
                                         ConfigSection::Model,  ConfigSection::Rules};
          std::string err;
          for (auto sec : order) {
            JsonObjectConst o = c.body()[configSectionName(sec)];
            if (o.isNull()) continue;
            err = sys.sectionFromJson(sec, o);
            if (!err.empty()) {
              err = std::string(configSectionName(sec)) + ": " + err;
              break;
            }
          }
          c.okOrFail(err);
        } else if (p.size() == 2 && parseSection(p[1], s) &&
                   (s == ConfigSection::System || s == ConfigSection::Network || s == ConfigSection::Mqtt)) {
          if (c.is("GET")) {
            sys.sectionToJson(s, out, false);
          } else if (c.is("PUT")) {
            std::string err = sys.sectionFromJson(s, c.body());
            if (err.empty() && sys.platformCommand) sys.platformCommand(std::string("apply_") + p[1], c.body());
            c.okOrFail(err);
          } else {
            c.fail(405, "Methode nicht erlaubt");
          }
        } else {
          c.fail(404, "unbekannter Konfigurationsabschnitt");
        }
      } else if (head == "system" && p.size() == 2 && c.is("POST")) {
        if (!sys.platformCommand) c.fail(501, "nicht verfuegbar");
        else c.okOrFail(sys.platformCommand(p[1], c.body()));
      } else {
        c.fail(404, "unbekannter Endpunkt");
      }
    }
  }
  ApiResponse r;
  r.status = c.status;
  serializeJson(c.out, r.body);
  return r;
}

}  // namespace hc
