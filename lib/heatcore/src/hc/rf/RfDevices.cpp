#include "hc/rf/RfDevices.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hc {
namespace rf {

const char* buttonRoleName(ButtonRole r) {
  switch (r) {
    case ButtonRole::StateOn: return "state_on";
    case ButtonRole::StateOff: return "state_off";
    default: return "button";
  }
}
ButtonRole buttonRoleFromName(const char* s) {
  if (s && std::strcmp(s, "state_on") == 0) return ButtonRole::StateOn;
  if (s && std::strcmp(s, "state_off") == 0) return ButtonRole::StateOff;
  return ButtonRole::Button;
}

const char* inputFunctionName(InputFunction f) {
  switch (f) {
    case InputFunction::Window: return "window";
    case InputFunction::Presence: return "presence";
    case InputFunction::HeatDemand: return "heat_demand";
    default: return "none";
  }
}
InputFunction inputFunctionFromName(const char* s) {
  if (!s) return InputFunction::None;
  if (std::strcmp(s, "window") == 0) return InputFunction::Window;
  if (std::strcmp(s, "presence") == 0) return InputFunction::Presence;
  if (std::strcmp(s, "heat_demand") == 0) return InputFunction::HeatDemand;
  return InputFunction::None;
}

std::string formatCode(uint32_t code, uint8_t bits) {
  char buf[24];
  int digits = (bits + 3) / 4;
  if (digits < 1) digits = 1;
  if (digits > 8) digits = 8;
  std::snprintf(buf, sizeof(buf), "0x%0*lX", digits, static_cast<unsigned long>(code));
  return buf;
}

bool parseCode(const char* s, uint32_t& out) {
  if (!s || !*s) return false;
  char* end = nullptr;
  unsigned long v = std::strtoul(s, &end, 0);
  if (!end || *end) return false;
  out = static_cast<uint32_t>(v);
  return true;
}

RxButton* RxDevice::button(uint8_t idx) {
  for (auto& b : buttons)
    if (b.index == idx) return &b;
  return nullptr;
}
const RxButton* RxDevice::button(uint8_t idx) const {
  for (auto& b : buttons)
    if (b.index == idx) return &b;
  return nullptr;
}

uint8_t RxDevice::reliability() const {
  if (stats.presses == 0) return 0;
  float rep = stats.framesPerPress / 4.f;  // >= 4 frames per press is excellent
  if (rep > 1.f) rep = 1.f;
  float total = stats.presses + stats.rejected;
  float accept = total > 0 ? stats.presses / total : 0.f;
  float score = 0.5f * stats.qualityAvg + 25.f * rep + 25.f * accept;
  if (score > 100.f) score = 100.f;
  return static_cast<uint8_t>(score);
}

TxCommand* TxDevice::command(uint16_t cid) {
  for (auto& c : commands)
    if (c.id == cid) return &c;
  return nullptr;
}
const TxCommand* TxDevice::command(uint16_t cid) const {
  for (auto& c : commands)
    if (c.id == cid) return &c;
  return nullptr;
}

RxDevice* RfRegistry::rxDevice(uint16_t id) {
  for (auto& d : rx)
    if (d.id == id) return &d;
  return nullptr;
}
const RxDevice* RfRegistry::rxDevice(uint16_t id) const {
  for (auto& d : rx)
    if (d.id == id) return &d;
  return nullptr;
}
TxDevice* RfRegistry::txDevice(uint16_t id) {
  for (auto& d : tx)
    if (d.id == id) return &d;
  return nullptr;
}
const TxDevice* RfRegistry::txDevice(uint16_t id) const {
  for (auto& d : tx)
    if (d.id == id) return &d;
  return nullptr;
}

ButtonRef RfRegistry::findByCode(uint32_t code, uint8_t bits) {
  for (auto& d : rx) {
    if (d.bits != bits) continue;
    for (auto& b : d.buttons)
      if (b.code == code) return {&d, &b};
  }
  return {};
}

bool RfRegistry::isIgnored(uint32_t code, uint8_t bits) const {
  for (auto& i : ignored)
    if (i.code == code && i.bits == bits) return true;
  return false;
}

bool RfRegistry::codeInUse(uint32_t code, uint8_t bits, uint16_t exceptDevice, std::string* owner) const {
  for (auto& d : rx) {
    if (d.id == exceptDevice || d.bits != bits) continue;
    for (auto& b : d.buttons) {
      if (b.code == code) {
        if (owner) *owner = d.name + " / " + b.name;
        return true;
      }
    }
  }
  return false;
}

uint16_t RfRegistry::nextRxId() const {
  uint16_t id = 1;
  for (auto& d : rx) id = std::max<uint16_t>(id, d.id + 1);
  return id;
}
uint16_t RfRegistry::nextTxId() const {
  uint16_t id = 1;
  for (auto& d : tx) id = std::max<uint16_t>(id, d.id + 1);
  return id;
}

std::string RfRegistry::upsertRx(RxDevice d, uint16_t* assignedId) {
  if (d.name.empty() || d.name.size() > 31) return "Name fehlt oder ist zu lang (max. 31 Zeichen)";
  if (d.protocol != Protocol::Ev1527 && d.protocol != Protocol::Pt2262)
    return "Nur EV1527 und PT2262 werden unterstuetzt";
  if (d.bits != 24) return "Nur 24-Bit-Codes werden unterstuetzt";
  if (d.pulseUs < 80 || d.pulseUs > 750) return "Pulslaenge ausserhalb 80..750 us";
  if (d.buttons.empty()) return "Mindestens eine Taste/ein Code erforderlich";
  if (d.buttons.size() > kMaxButtonsPerDevice) return "Zu viele Tasten";
  if (d.minRepeats < 1 || d.minRepeats > 10) return "Mindestwiederholungen 1..10";
  if (d.longPressMs < 300 || d.longPressMs > 10000) return "Langer Tastendruck 300..10000 ms";
  if (d.releaseMs < 100 || d.releaseMs > 2000) return "Loslasszeit 100..2000 ms";
  if (d.retriggerMs > 5000) return "Entprellzeit max. 5000 ms";
  if (d.supervisionSec != 0 && (d.supervisionSec < 60 || d.supervisionSec > 7 * 86400))
    return "Ueberwachungszeit 0 oder 60 s..7 Tage";
  if (d.function != InputFunction::None && d.room.empty()) return "Funktion erfordert eine Raumzuordnung";
  for (size_t i = 0; i < d.buttons.size(); ++i) {
    auto& b = d.buttons[i];
    if (b.name.empty()) b.name = "Taste " + std::to_string(b.index);
    for (size_t j = i + 1; j < d.buttons.size(); ++j) {
      if (d.buttons[j].code == b.code) return "Code " + formatCode(b.code, d.bits) + " doppelt vergeben";
      if (d.buttons[j].index == b.index) return "Tastenindex doppelt";
    }
    std::string owner;
    if (codeInUse(b.code, d.bits, d.id, &owner))
      return "Code " + formatCode(b.code, d.bits) + " bereits belegt durch " + owner;
    if (isIgnored(b.code, d.bits)) return "Code " + formatCode(b.code, d.bits) + " steht auf der Ignorierliste";
  }
  if (d.id == 0) {
    if (rx.size() >= kMaxRxDevices) return "Maximale Geraeteanzahl erreicht";
    d.id = nextRxId();
    d.address = splitCode(d.protocol, d.buttons.front().code, d.bits).address;
    rx.push_back(std::move(d));
    if (assignedId) *assignedId = rx.back().id;
  } else {
    RxDevice* existing = rxDevice(d.id);
    if (!existing) return "Geraet nicht gefunden";
    // keep statistics and runtime state across configuration edits
    d.stats = existing->stats;
    d.state = existing->state;
    d.available = existing->available;
    d.stateOnMs = existing->stateOnMs;
    for (auto& b : d.buttons) {
      if (const RxButton* old = existing->button(b.index)) {
        if (old->code == b.code) {
          b.count = old->count;
          b.lastEpoch = old->lastEpoch;
        }
      }
    }
    d.address = splitCode(d.protocol, d.buttons.front().code, d.bits).address;
    if (assignedId) *assignedId = d.id;
    *existing = std::move(d);
  }
  touch();
  return std::string();
}

bool RfRegistry::removeRx(uint16_t id) {
  auto it = std::remove_if(rx.begin(), rx.end(), [&](const RxDevice& d) { return d.id == id; });
  if (it == rx.end()) return false;
  rx.erase(it, rx.end());
  touch();
  return true;
}

std::string RfRegistry::upsertTx(TxDevice d, uint16_t* assignedId) {
  if (d.name.empty() || d.name.size() > 31) return "Name fehlt oder ist zu lang (max. 31 Zeichen)";
  if (d.protocol != Protocol::Ev1527 && d.protocol != Protocol::Pt2262)
    return "Nur EV1527 und PT2262 koennen gesendet werden";
  if (d.pulseUs < 80 || d.pulseUs > 750) return "Pulslaenge ausserhalb 80..750 us";
  if (d.repeats < 1 || d.repeats > kMaxRepeats) return "Wiederholungen 1..20";
  if (d.group.size() > 23) return "Gruppenname zu lang";
  uint16_t nextCmd = 1;
  for (auto& c : d.commands) nextCmd = std::max<uint16_t>(nextCmd, c.id + 1);
  for (size_t i = 0; i < d.commands.size(); ++i) {
    auto& c = d.commands[i];
    if (c.id == 0) c.id = nextCmd++;
    if (c.name.empty() || c.name.size() > 31) return "Befehlsname fehlt oder ist zu lang";
    if (c.bits != 24) return "Nur 24-Bit-Codes";
    if (c.repeats > kMaxRepeats) return "Wiederholungen max. 20";
    for (size_t j = i + 1; j < d.commands.size(); ++j)
      if (d.commands[j].id == c.id) return "Befehls-ID doppelt";
  }
  if (d.id == 0) {
    if (tx.size() >= kMaxTxDevices) return "Maximale Senderanzahl erreicht";
    d.id = nextTxId();
    tx.push_back(std::move(d));
    if (assignedId) *assignedId = tx.back().id;
  } else {
    TxDevice* existing = txDevice(d.id);
    if (!existing) return "Sender nicht gefunden";
    for (auto& c : d.commands) {
      if (const TxCommand* old = existing->command(c.id)) {
        c.sent = old->sent;
        c.failed = old->failed;
        c.lastEpoch = old->lastEpoch;
      }
    }
    if (assignedId) *assignedId = d.id;
    *existing = std::move(d);
  }
  touch();
  return std::string();
}

bool RfRegistry::removeTx(uint16_t id) {
  auto it = std::remove_if(tx.begin(), tx.end(), [&](const TxDevice& d) { return d.id == id; });
  if (it == tx.end()) return false;
  tx.erase(it, tx.end());
  touch();
  return true;
}

std::string RfRegistry::addIgnored(uint32_t code, uint8_t bits, const std::string& note) {
  if (isIgnored(code, bits)) return std::string();
  std::string owner;
  if (codeInUse(code, bits, 0, &owner)) return "Code gehoert zu " + owner + " - zuerst Taste entfernen";
  if (ignored.size() >= kMaxIgnored) return "Ignorierliste voll";
  ignored.push_back({code, bits, note.substr(0, 31)});
  touch();
  return std::string();
}

bool RfRegistry::removeIgnored(uint32_t code, uint8_t bits) {
  auto it = std::remove_if(ignored.begin(), ignored.end(),
                           [&](const IgnoredCode& i) { return i.code == code && i.bits == bits; });
  if (it == ignored.end()) return false;
  ignored.erase(it, ignored.end());
  touch();
  return true;
}

std::vector<RfRegistry::TxTarget> RfRegistry::resolveTx(const std::string& ref, bool forAutomation,
                                                        std::string* err) const {
  std::vector<TxTarget> out;
  auto fail = [&](const char* m) {
    if (err) *err = m;
    return std::vector<TxTarget>{};
  };
  size_t slash = ref.rfind('/');
  if (slash == std::string::npos) return fail("Befehlsreferenz muss <Sender>/<Befehl> lauten");
  std::string left = ref.substr(0, slash), right = ref.substr(slash + 1);
  if (left.rfind("group:", 0) == 0) {
    std::string group = left.substr(6);
    for (auto& d : tx) {
      if (d.group != group || !d.enabled) continue;
      for (auto& c : d.commands)
        if (c.name == right && (!forAutomation || c.automationAllowed)) out.push_back({&d, &c});
    }
    if (out.empty()) return fail("Kein passender Befehl in der Gruppe");
    return out;
  }
  int devId = std::atoi(left.c_str());
  int cmdId = std::atoi(right.c_str());
  const TxDevice* d = txDevice(static_cast<uint16_t>(devId));
  if (!d) return fail("Sender nicht gefunden");
  if (!d->enabled) return fail("Sender deaktiviert");
  const TxCommand* c = d->command(static_cast<uint16_t>(cmdId));
  if (!c) return fail("Befehl nicht gefunden");
  if (forAutomation && !c->automationAllowed) return fail("Befehl ist fuer Automationen gesperrt");
  out.push_back({d, c});
  return out;
}

std::vector<std::string> RfRegistry::groups() const {
  std::vector<std::string> g;
  for (auto& d : tx)
    if (!d.group.empty() && std::find(g.begin(), g.end(), d.group) == g.end()) g.push_back(d.group);
  return g;
}

// ---------------------------------------------------------------- JSON

void RfRegistry::rxDeviceToJson(const RxDevice& d, JsonObject o, bool includeStats) {
  o["id"] = d.id;
  o["name"] = d.name;
  o["protocol"] = protocolName(d.protocol);
  o["bits"] = d.bits;
  o["pulseUs"] = d.pulseUs;
  o["address"] = formatCode(d.address, splitCode(d.protocol, 0, d.bits).addressBits);
  o["enabled"] = d.enabled;
  o["function"] = inputFunctionName(d.function);
  o["room"] = d.room;
  o["minRepeats"] = d.minRepeats;
  o["longPressMs"] = d.longPressMs;
  o["releaseMs"] = d.releaseMs;
  o["retriggerMs"] = d.retriggerMs;
  o["supervisionSec"] = d.supervisionSec;
  o["failsafeOn"] = d.failsafeOn;
  o["autoOffSec"] = d.autoOffSec;
  JsonArray bs = o["buttons"].to<JsonArray>();
  for (auto& b : d.buttons) {
    JsonObject bo = bs.add<JsonObject>();
    bo["index"] = b.index;
    bo["name"] = b.name;
    bo["code"] = formatCode(b.code, d.bits);
    bo["role"] = buttonRoleName(b.role);
    std::string tri = d.protocol == Protocol::Pt2262 ? triStateString(b.code, d.bits) : std::string();
    if (!tri.empty()) bo["tristate"] = tri;
    bindingsToJson(b.bindings, bo["bindings"].to<JsonArray>());
    if (includeStats) {
      bo["count"] = b.count;
      bo["lastEpoch"] = b.lastEpoch;
    }
  }
  if (includeStats) {
    JsonObject s = o["stats"].to<JsonObject>();
    s["frames"] = d.stats.frames;
    s["presses"] = d.stats.presses;
    s["rejected"] = d.stats.rejected;
    s["timingMismatch"] = d.stats.timingMismatch;
    s["lastEpoch"] = d.stats.lastEpoch;
    s["quality"] = static_cast<int>(d.stats.qualityAvg + 0.5f);
    s["framesPerPress"] = d.stats.framesPerPress;
    s["pulseUsAvg"] = static_cast<int>(d.stats.pulseUsAvg + 0.5f);
    s["reliability"] = d.reliability();
  }
}

std::string RfRegistry::rxDeviceFromJson(RxDevice& d, JsonObjectConst o) {
  d.id = o["id"] | 0;
  d.name = o["name"] | "";
  d.protocol = protocolFromName(o["protocol"] | "ev1527");
  d.bits = o["bits"] | 24;
  d.pulseUs = o["pulseUs"] | 350;
  d.enabled = o["enabled"] | true;
  d.function = inputFunctionFromName(o["function"] | "none");
  d.room = o["room"] | "";
  d.minRepeats = o["minRepeats"] | 2;
  d.longPressMs = o["longPressMs"] | 1000;
  d.releaseMs = o["releaseMs"] | 250;
  d.retriggerMs = o["retriggerMs"] | 300;
  d.supervisionSec = o["supervisionSec"] | 0u;
  d.failsafeOn = o["failsafeOn"] | false;
  d.autoOffSec = o["autoOffSec"] | 0u;
  d.buttons.clear();
  uint8_t autoIdx = 1;
  for (JsonObjectConst bo : o["buttons"].as<JsonArrayConst>()) {
    RxButton b;
    b.index = bo["index"] | autoIdx;
    autoIdx = static_cast<uint8_t>(std::max<int>(autoIdx, b.index) + 1);
    b.name = bo["name"] | "";
    if (!parseCode(bo["code"] | "", b.code)) return "Ungueltiger Code";
    b.role = buttonRoleFromName(bo["role"] | "button");
    std::string err;
    if (!bindingsFromJson(b.bindings, bo["bindings"].as<JsonArrayConst>(), err)) return err;
    b.count = bo["count"] | 0u;
    b.lastEpoch = bo["lastEpoch"] | static_cast<int64_t>(0);
    d.buttons.push_back(std::move(b));
  }
  JsonObjectConst s = o["stats"];
  if (!s.isNull()) {
    d.stats.frames = s["frames"] | 0u;
    d.stats.presses = s["presses"] | 0u;
    d.stats.rejected = s["rejected"] | 0u;
    d.stats.timingMismatch = s["timingMismatch"] | 0u;
    d.stats.lastEpoch = s["lastEpoch"] | static_cast<int64_t>(0);
    d.stats.qualityAvg = s["quality"] | 0.f;
    d.stats.framesPerPress = s["framesPerPress"] | 0.f;
    d.stats.pulseUsAvg = s["pulseUsAvg"] | 0.f;
  }
  return std::string();
}

void RfRegistry::txDeviceToJson(const TxDevice& d, JsonObject o) {
  o["id"] = d.id;
  o["name"] = d.name;
  o["group"] = d.group;
  o["protocol"] = protocolName(d.protocol);
  o["pulseUs"] = d.pulseUs;
  o["repeats"] = d.repeats;
  o["enabled"] = d.enabled;
  JsonArray cs = o["commands"].to<JsonArray>();
  for (auto& c : d.commands) {
    JsonObject co = cs.add<JsonObject>();
    co["id"] = c.id;
    co["name"] = c.name;
    co["code"] = formatCode(c.code, c.bits);
    co["bits"] = c.bits;
    co["repeats"] = c.repeats;
    co["automationAllowed"] = c.automationAllowed;
    co["haExposed"] = c.haExposed;
    co["sent"] = c.sent;
    co["failed"] = c.failed;
    co["lastEpoch"] = c.lastEpoch;
  }
}

std::string RfRegistry::txDeviceFromJson(TxDevice& d, JsonObjectConst o) {
  d.id = o["id"] | 0;
  d.name = o["name"] | "";
  d.group = o["group"] | "";
  d.protocol = protocolFromName(o["protocol"] | "ev1527");
  d.pulseUs = o["pulseUs"] | 350;
  d.repeats = o["repeats"] | 8;
  d.enabled = o["enabled"] | true;
  d.commands.clear();
  for (JsonObjectConst co : o["commands"].as<JsonArrayConst>()) {
    TxCommand c;
    c.id = co["id"] | 0;
    c.name = co["name"] | "";
    if (!parseCode(co["code"] | "", c.code)) return "Ungueltiger Befehlscode";
    c.bits = co["bits"] | 24;
    c.repeats = co["repeats"] | 0;
    c.automationAllowed = co["automationAllowed"] | true;
    c.haExposed = co["haExposed"] | false;
    c.sent = co["sent"] | 0u;
    c.failed = co["failed"] | 0u;
    c.lastEpoch = co["lastEpoch"] | static_cast<int64_t>(0);
    d.commands.push_back(std::move(c));
  }
  return std::string();
}

void RfRegistry::toJson(JsonObject o, bool includeStats) const {
  JsonArray r = o["rx"].to<JsonArray>();
  for (auto& d : rx) rxDeviceToJson(d, r.add<JsonObject>(), includeStats);
  JsonArray t = o["tx"].to<JsonArray>();
  for (auto& d : tx) txDeviceToJson(d, t.add<JsonObject>());
  JsonArray ig = o["ignored"].to<JsonArray>();
  for (auto& i : ignored) {
    JsonObject io = ig.add<JsonObject>();
    io["code"] = formatCode(i.code, i.bits);
    io["bits"] = i.bits;
    io["note"] = i.note;
  }
}

std::string RfRegistry::fromJson(JsonObjectConst o) {
  std::vector<RxDevice> newRx;
  for (JsonObjectConst d : o["rx"].as<JsonArrayConst>()) {
    RxDevice dev;
    std::string e = rxDeviceFromJson(dev, d);
    if (!e.empty()) return e;
    if (dev.id == 0) continue;
    dev.address = splitCode(dev.protocol, dev.buttons.empty() ? 0 : dev.buttons.front().code, dev.bits).address;
    newRx.push_back(std::move(dev));
  }
  std::vector<TxDevice> newTx;
  for (JsonObjectConst d : o["tx"].as<JsonArrayConst>()) {
    TxDevice dev;
    std::string e = txDeviceFromJson(dev, d);
    if (!e.empty()) return e;
    if (dev.id == 0) continue;
    newTx.push_back(std::move(dev));
  }
  std::vector<IgnoredCode> newIg;
  for (JsonObjectConst i : o["ignored"].as<JsonArrayConst>()) {
    IgnoredCode ic;
    if (!parseCode(i["code"] | "", ic.code)) continue;
    ic.bits = i["bits"] | 24;
    ic.note = i["note"] | "";
    newIg.push_back(ic);
  }
  rx = std::move(newRx);
  tx = std::move(newTx);
  ignored = std::move(newIg);
  touch();
  return std::string();
}

}  // namespace rf
}  // namespace hc
