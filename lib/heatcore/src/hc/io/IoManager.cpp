#include "hc/io/IoManager.h"

#include <cstdio>
#include <cstring>

namespace hc {

const char* outputFunctionName(OutputFunction f) {
  switch (f) {
    case OutputFunction::Circuit: return "circuit";
    case OutputFunction::General: return "general";
    case OutputFunction::Disabled: return "disabled";
  }
  return "?";
}
OutputFunction outputFunctionFromName(const char* s) {
  if (s && !std::strcmp(s, "general")) return OutputFunction::General;
  if (s && !std::strcmp(s, "disabled")) return OutputFunction::Disabled;
  return OutputFunction::Circuit;
}
const char* inputKindName(InputKind k) {
  switch (k) {
    case InputKind::Button: return "button";
    case InputKind::Contact: return "contact";
    case InputKind::Disabled: return "disabled";
  }
  return "?";
}
InputKind inputKindFromName(const char* s) {
  if (s && !std::strcmp(s, "contact")) return InputKind::Contact;
  if (s && !std::strcmp(s, "disabled")) return InputKind::Disabled;
  return InputKind::Button;
}

IoManager::IoManager(EventBus& bus, const IClock& clock) : bus_(bus), clock_(clock) {
  for (int i = 0; i < kNumOutputs; ++i) outCfg[i].name = "Ausgang " + std::to_string(i + 1);
  for (int i = 0; i < kNumInputs; ++i) inCfg[i].name = "Eingang " + std::to_string(i + 1);
  for (int i = 0; i < kNumAnalog; ++i) {
    aiCfg[i].name = "A" + std::to_string(i + 1);
    // 0..5 V inputs: 5.1 kOhm / 10 kOhm divider on the KC868-A16 (schematic),
    // i.e. pin voltage x 1.51. Calibrate with a known voltage.
    aiCfg[i].dividerGain = 1.51f;
  }
}

bool IoManager::inputLogical(int idx) const { return in[idx].stable != inCfg[idx].invert; }

void IoManager::emitInput(int idx, Gesture g, Millis now) {
  Event e(EventType::InputGesture, EventSource::DigitalInput);
  e.ts = now;
  e.gesture = g;
  e.a = idx + 1;
  char ref[24];
  std::snprintf(ref, sizeof(ref), "di:%d", idx + 1);
  e.setRef(ref);
  e.value2 = inputLogical(idx) ? 1.f : 0.f;
  bus_.post(e);
}

void IoManager::onInputs(uint16_t activeMask, Millis now) {
  for (int i = 0; i < kNumInputs; ++i) {
    InputConfig& c = inCfg[i];
    InputState& s = in[i];
    bool raw = (activeMask >> i) & 1u;
    if (c.kind == InputKind::Disabled) {
      s.raw = s.stable = raw;
      s.known = true;
      continue;
    }
    if (raw != s.raw) {
      s.raw = raw;
      s.rawChangeMs = now;
      ++s.edges;
    }
    uint16_t deb = c.debounceMs < 10 ? 10 : c.debounceMs;
    bool settled = elapsed(now, s.rawChangeMs) >= deb;
    if (!s.known) {
      if (!settled && s.edges > 0) continue;
      s.stable = s.raw;
      s.known = true;
      if (c.kind == InputKind::Contact) emitInput(i, inputLogical(i) ? Gesture::Open : Gesture::Closed, now);
      continue;
    }
    if (!settled || s.raw == s.stable) continue;
    s.stable = s.raw;
    if (c.kind == InputKind::Contact) {
      emitInput(i, inputLogical(i) ? Gesture::Open : Gesture::Closed, now);
    } else if (inputLogical(i)) {
      s.pressStartMs = now;
      s.longSent = false;
      emitInput(i, Gesture::Press, now);
    } else {
      if (!s.longSent) emitInput(i, Gesture::Short, now);
      emitInput(i, Gesture::Release, now);
    }
  }
}

void IoManager::tickInputs(Millis now) {
  for (int i = 0; i < kNumInputs; ++i) {
    InputState& s = in[i];
    const InputConfig& c = inCfg[i];
    if (c.kind != InputKind::Button || !s.known || !inputLogical(i) || s.longSent) continue;
    if (elapsed(now, s.pressStartMs) >= c.longPressMs) {
      s.longSent = true;
      emitInput(i, Gesture::Long, now);
    }
  }
}

void IoManager::onAnalog(int idx, float pinVolts, Millis now) {
  if (idx < 0 || idx >= kNumAnalog) return;
  AnalogState& a = ai[idx];
  const AnalogConfig& c = aiCfg[idx];
  a.pinVolts = pinVolts;
  a.volts = pinVolts * c.dividerGain;
  a.value = a.volts * c.scale + c.offset;
  a.valid = true;
  a.lastMs = now;
}

void IoManager::setRequest(int idx, bool on, Millis now) {
  OutputState& s = out[idx];
  if (on && !s.request) s.requestOnMs = now;
  s.request = on;
}

void IoManager::setManual(int idx, bool on, Millis until, EventSource src) {
  OutputState& s = out[idx];
  s.manualActive = true;
  s.manualState = on;
  s.manualUntil = until;
  s.manualSource = src;
}

void IoManager::clearManual(int idx) { out[idx].manualActive = false; }

uint16_t IoManager::levelMask() const {
  uint16_t m = 0;
  for (int i = 0; i < kNumOutputs; ++i)
    if (out[i].level) m |= static_cast<uint16_t>(1u << i);
  return m;
}

bool IoManager::applyLevel(int idx, bool level, Millis now) {
  OutputState& s = out[idx];
  if (elapsed(now, s.hourStart) >= 3600u * 1000u) {
    s.hourStart = now;
    s.switchesThisHour = 0;
  }
  if (s.level == level && s.everChanged) return false;
  bool changed = s.level != level;
  s.level = level;
  s.everChanged = true;
  if (!changed) return false;
  s.lastChangeMs = now;
  ++s.switches;
  ++s.switchesThisHour;
  Event e(EventType::OutputChanged, EventSource::System);
  e.ts = now;
  e.a = idx + 1;
  e.value = level ? 1.f : 0.f;
  bus_.post(e);
  return true;
}

void IoManager::toJson(JsonObject o, bool runtime) const {
  Millis now = clock_.nowMs();
  JsonArray outs = o["outputs"].to<JsonArray>();
  for (int i = 0; i < kNumOutputs; ++i) {
    JsonObject x = outs.add<JsonObject>();
    x["n"] = i + 1;
    x["name"] = outCfg[i].name;
    x["function"] = outputFunctionName(outCfg[i].function);
    x["automationAllowed"] = outCfg[i].automationAllowed;
    x["haAllowed"] = outCfg[i].haAllowed;
    x["maxOnSec"] = outCfg[i].maxOnSec;
    if (runtime) {
      const OutputState& s = out[i];
      x["on"] = s.level;
      x["request"] = s.request;
      x["manual"] = s.manualActive;
      if (s.manualActive) {
        x["manualState"] = s.manualState;
        x["manualRemainingSec"] = reached(now, s.manualUntil) ? 0u : (s.manualUntil - now) / 1000u;
      }
      x["switches"] = s.switches;
      x["switchesThisHour"] = s.switchesThisHour;
      x["hwOk"] = s.hwOk;
      x["blocked"] = s.blocked;
    }
  }
  JsonArray ins = o["inputs"].to<JsonArray>();
  for (int i = 0; i < kNumInputs; ++i) {
    JsonObject x = ins.add<JsonObject>();
    x["n"] = i + 1;
    x["name"] = inCfg[i].name;
    x["kind"] = inputKindName(inCfg[i].kind);
    x["invert"] = inCfg[i].invert;
    x["function"] = rf::inputFunctionName(inCfg[i].function);
    x["room"] = inCfg[i].room;
    x["debounceMs"] = inCfg[i].debounceMs;
    x["longPressMs"] = inCfg[i].longPressMs;
    bindingsToJson(inCfg[i].bindings, x["bindings"].to<JsonArray>());
    if (runtime) {
      x["active"] = in[i].stable;
      x["logical"] = inputLogical(i);
      x["edges"] = in[i].edges;
    }
  }
  JsonArray ais = o["analog"].to<JsonArray>();
  for (int i = 0; i < kNumAnalog; ++i) {
    JsonObject x = ais.add<JsonObject>();
    x["n"] = i + 1;
    x["name"] = aiCfg[i].name;
    x["enabled"] = aiCfg[i].enabled;
    x["asSensor"] = aiCfg[i].asSensor;
    x["dividerGain"] = aiCfg[i].dividerGain;
    x["scale"] = aiCfg[i].scale;
    x["offset"] = aiCfg[i].offset;
    x["unit"] = aiCfg[i].unit;
    if (runtime && ai[i].valid) {
      x["pinVolts"] = ai[i].pinVolts;
      x["volts"] = ai[i].volts;
      x["value"] = ai[i].value;
    }
  }
}

std::string IoManager::fromJson(JsonObjectConst o) {
  std::string err;
  for (JsonObjectConst x : o["outputs"].as<JsonArrayConst>()) {
    int n = x["n"] | 0;
    if (n < 1 || n > kNumOutputs) continue;
    OutputConfig& c = outCfg[n - 1];
    c.name = x["name"] | c.name.c_str();
    c.function = outputFunctionFromName(x["function"] | "circuit");
    c.automationAllowed = c.function == OutputFunction::General && (x["automationAllowed"] | false);
    c.haAllowed = c.function == OutputFunction::General && (x["haAllowed"] | false);
    c.maxOnSec = x["maxOnSec"] | 0u;
  }
  for (JsonObjectConst x : o["inputs"].as<JsonArrayConst>()) {
    int n = x["n"] | 0;
    if (n < 1 || n > kNumInputs) continue;
    InputConfig c;
    c.name = x["name"] | inCfg[n - 1].name.c_str();
    c.kind = inputKindFromName(x["kind"] | "button");
    c.invert = x["invert"] | false;
    c.function = rf::inputFunctionFromName(x["function"] | "none");
    c.room = x["room"] | "";
    c.debounceMs = x["debounceMs"] | (c.kind == InputKind::Contact ? 200 : 50);
    c.longPressMs = x["longPressMs"] | 1000;
    if (c.debounceMs > 5000) return "Entprellzeit max. 5000 ms";
    if (c.longPressMs < 300 || c.longPressMs > 10000) return "Langer Tastendruck 300..10000 ms";
    if (c.function != rf::InputFunction::None && c.room.empty()) return "Funktion erfordert eine Raumzuordnung";
    if (!bindingsFromJson(c.bindings, x["bindings"].as<JsonArrayConst>(), err)) return err;
    inCfg[n - 1] = std::move(c);
  }
  for (JsonObjectConst x : o["analog"].as<JsonArrayConst>()) {
    int n = x["n"] | 0;
    if (n < 1 || n > kNumAnalog) continue;
    AnalogConfig& c = aiCfg[n - 1];
    c.name = x["name"] | c.name.c_str();
    c.enabled = x["enabled"] | false;
    c.asSensor = x["asSensor"] | false;
    c.dividerGain = x["dividerGain"] | c.dividerGain;
    c.scale = x["scale"] | 1.f;
    c.offset = x["offset"] | 0.f;
    c.unit = x["unit"] | "V";
  }
  return std::string();
}

}  // namespace hc
